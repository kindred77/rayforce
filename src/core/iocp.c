/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.

 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:

 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.

 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

#include "core/poll.h"
#include <stdio.h>

#if defined(RAY_OS_WINDOWS)

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mswsock.h>

#include "mem/sys.h"
#include "core/timer.h"

#define RAY_POLL_MAX_EVENTS 64
#define RAY_POLL_INITIAL_CAP 16

// IOCP 每个异步操作的重叠上下文
typedef struct {
    OVERLAPPED overlapped;
    ray_selector_t* sel;
    WSABUF wsabuf;
    char buf[4096];
} ray_iocp_ov_t;

ray_poll_t* ray_poll_create(void) {
    // 创建完成端口
    HANDLE iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (iocp == NULL) {
        fprintf(stderr, "CreateIoCompletionPort failed %lu\n", GetLastError());
        return NULL;
    }

    ray_poll_t* poll = (ray_poll_t*)ray_sys_alloc(sizeof(ray_poll_t));
    if (!poll) {
        CloseHandle(iocp);
        return NULL;
    }
    memset(poll, 0, sizeof(*poll));

    // Linux下poll->fd存epoll fd，Windows复用该字段存IOCP句柄
    poll->fd = (uint64_t)iocp;
    poll->code = -1;
    poll->sel_cap = RAY_POLL_INITIAL_CAP;
    poll->sels = (ray_selector_t**)ray_sys_alloc(poll->sel_cap * sizeof(ray_selector_t*));
    if (!poll->sels) {
        CloseHandle((HANDLE)poll->fd);
        ray_sys_free(poll);
        return NULL;
    }
    memset(poll->sels, 0, poll->sel_cap * sizeof(ray_selector_t*));

    return poll;
}

void ray_poll_destroy(ray_poll_t* poll)
{
    if (!poll) return;

    // 遍历释放所有selector，对齐Linux清理逻辑
    for (uint32_t i = 0; i < poll->n_sels; ++i) {
        ray_selector_t* sel = poll->sels[i];
        if (!sel) continue;

        if (sel->close_fn)
            sel->close_fn(poll, sel);

        // 关闭socket自动从IOCP解绑
        if ((SOCKET)sel->fd != INVALID_SOCKET)
            closesocket((SOCKET)sel->fd);

        if (sel->rx.buf)
            ray_poll_buf_free(sel->rx.buf);
        ray_poll_buf_free(sel->tx.buf);
        ray_sys_free(sel);
        poll->sels[i] = NULL;
    }

    if (poll->sels)
        ray_sys_free(poll->sels);

    if ((HANDLE)poll->fd != NULL)
        CloseHandle((HANDLE)poll->fd);

    if (poll->timers) {
        ray_timers_destroy((ray_timers_t*)poll->timers);
        poll->timers = NULL;
    }

    ray_sys_free(poll);
}

// 内部：给socket投递异步WSARecv，监听可读事件
static int ray_poll_post_recv(ray_poll_t* poll, ray_selector_t* sel)
{
    ray_iocp_ov_t* ov = (ray_iocp_ov_t*)ray_sys_alloc(sizeof(ray_iocp_ov_t));
    if (!ov) return -1;

    ZeroMemory(&ov->overlapped, sizeof(OVERLAPPED));
    ov->sel = sel;
    ov->wsabuf.buf = ov->buf;
    ov->wsabuf.len = sizeof(ov->buf);

    DWORD flags = 0;
    DWORD recv_len = 0;
    int ret = WSARecv((SOCKET)sel->fd, &ov->wsabuf, 1, &recv_len, &flags, &ov->overlapped, NULL);

    if (ret == 0) {
        // 同步立即收到数据，手动入队触发完成事件
        PostQueuedCompletionStatus((HANDLE)poll->fd, recv_len, (ULONG_PTR)sel, &ov->overlapped);
    } else {
        int err = WSAGetLastError();
        if (err != WSA_IO_PENDING) {
            ray_sys_free(ov);
            return -1;
        }
    }
    return 0;
}

int64_t ray_poll_register(ray_poll_t* poll, ray_poll_reg_t* reg)
{
    if (!poll || !reg) return -1;

    // 查找空闲槽位
    int64_t id = -1;
    for (uint32_t i = 0; i < poll->n_sels; ++i) {
        if (!poll->sels[i]) {
            id = (int64_t)i;
            break;
        }
    }

    // 容量不足则扩容
    if (id < 0) {
        if (poll->n_sels >= poll->sel_cap) {
            uint32_t new_cap = poll->sel_cap * 2;
            ray_selector_t** ns = (ray_selector_t**)ray_sys_alloc(new_cap * sizeof(ray_selector_t*));
            if (!ns) return -1;

            memcpy(ns, poll->sels, poll->n_sels * sizeof(ray_selector_t*));
            memset(ns + poll->n_sels, 0, (new_cap - poll->n_sels) * sizeof(ray_selector_t*));
            ray_sys_free(poll->sels);
            poll->sels = ns;
            poll->sel_cap = new_cap;
        }
        id = (int64_t)poll->n_sels++;
    }

    ray_selector_t* sel = (ray_selector_t*)ray_sys_alloc(sizeof(ray_selector_t));
    if (!sel) return -1;
    memset(sel, 0, sizeof(*sel));

    sel->fd = reg->fd;
    sel->id = id;
    sel->type = reg->type;
    sel->data = reg->data;
    sel->open_fn = reg->open_fn;
    sel->close_fn = reg->close_fn;
    sel->error_fn = reg->error_fn;
    sel->data_fn = reg->data_fn;
    sel->rx.recv_fn = reg->recv_fn;
    sel->rx.read_fn = reg->read_fn;
    sel->tx.send_fn = reg->send_fn;

    poll->sels[id] = sel;

    // Socket绑定IOCP
    HANDLE ret = CreateIoCompletionPort((HANDLE)sel->fd, (HANDLE)poll->fd, (ULONG_PTR)sel, 0);
    if (ret == NULL) {
        poll->sels[id] = NULL;
        ray_sys_free(sel);
        return -1;
    }

    // 投递异步recv，监听可读（等价epoll EPOLLIN）
    if (ray_poll_post_recv(poll, sel) != 0) {
        poll->sels[id] = NULL;
        ray_sys_free(sel);
        return -1;
    }

    if (sel->open_fn)
        sel->open_fn(poll, sel);

    return id;
}

void ray_poll_deregister(ray_poll_t* poll, int64_t id)
{
    if (!poll || id < 0 || (uint32_t)id >= poll->n_sels) return;
    ray_selector_t* sel = poll->sels[id];
    if (!sel) return;

    // IOCP无需手动删除句柄，关闭socket自动解绑
    if (sel->close_fn)
        sel->close_fn(poll, sel);

    if ((SOCKET)sel->fd != INVALID_SOCKET)
        closesocket((SOCKET)sel->fd);

    if (sel->rx.buf)
        ray_poll_buf_free(sel->rx.buf);
    ray_poll_buf_free(sel->tx.buf);
    ray_sys_free(sel);
    poll->sels[id] = NULL;
}

int64_t ray_poll_run(ray_poll_t* poll)
{
    if (!poll) return -1;
    HANDLE iocp = (HANDLE)poll->fd;

    while (poll->code < 0) {
        DWORD wait_ms = INFINITE;
        // 计算定时器超时时间，对齐Linux逻辑
        if (poll->timers) {
            int64_t deadline = ray_timers_next_deadline_ms((ray_timers_t*)poll->timers);
            if (deadline != INT64_MAX) {
                int64_t now = ray_time_now_ms();
                int64_t delta = deadline - now;
                if (delta < 0) delta = 0;
                if (delta > INT_MAX) delta = INT_MAX;
                wait_ms = (DWORD)delta;
            }
        }

        DWORD bytes_xfer = 0;
        ULONG_PTR key = 0;
        LPOVERLAPPED ov_ptr = NULL;

        BOOL ok = GetQueuedCompletionStatus(iocp, &bytes_xfer, &key, &ov_ptr, wait_ms);
        if (!ok) {
            DWORD err = GetLastError();
            // 超时，触发定时器
            if (err == WAIT_TIMEOUT) {
                ray_timers_fire_expired((ray_timers_t*)poll->timers);
                continue;
            }
            // 其他错误直接退出
            return -1;
        }

        ray_iocp_ov_t* ov = CONTAINING_RECORD(ov_ptr, ray_iocp_ov_t, overlapped);
        ray_selector_t* sel = ov->sel;
        uint64_t eid = sel->id;

        // 校验selector是否已经被销毁
        if (eid >= poll->n_sels || poll->sels[eid] != sel) {
            ray_sys_free(ov);
            goto fire_timer;
        }

        // 连接关闭 / 读取异常
        if (bytes_xfer == 0) {
            if (sel->error_fn)
                sel->error_fn(poll, sel);
            else
                ray_poll_deregister(poll, eid);
            ray_sys_free(ov);
            goto fire_timer;
        }

        // 把本次读到的数据写入rx buffer，复用你原有读循环逻辑
        if (sel->rx.buf && sel->rx.recv_fn) {
            size_t copy_len = bytes_xfer;
            size_t off = sel->rx.buf->offset;
            if (off + copy_len > sel->rx.buf->size)
                copy_len = sel->rx.buf->size - off;

            memcpy(sel->rx.buf->data + off, ov->buf, copy_len);
            sel->rx.buf->offset += copy_len;

            // 原Linux业务循环：反复读、状态机、回调
            for (;;) {
                if (sel->rx.buf->offset < sel->rx.buf->size)
                    break;

                if (!sel->rx.read_fn) break;
                ray_t* obj = sel->rx.read_fn(poll, sel);

                if (eid >= poll->n_sels || poll->sels[eid] != sel)
                    break;

                if (obj && sel->data_fn)
                    sel->data_fn(poll, sel, obj);

                if (eid >= poll->n_sels || poll->sels[eid] != sel)
                    break;

                if (!sel->rx.buf) break;
                if (sel->rx.buf->offset >= sel->rx.buf->size)
                    continue;
                break;
            }
        }

        // 重新投递异步Recv，持续监听下一次可读事件
        ZeroMemory(&ov->overlapped, sizeof(OVERLAPPED));
        ov->wsabuf.len = sizeof(ov->buf);
        DWORD flags = 0;
        DWORD dummy;
        WSARecv((SOCKET)sel->fd, &ov->wsabuf, 1, &dummy, &flags, &ov->overlapped, NULL);

fire_timer:
        // 执行到期定时器
        if (poll->timers)
            ray_timers_fire_expired((ray_timers_t*)poll->timers);
    }

    return poll->code;
}

#endif /* _WIN32 */
