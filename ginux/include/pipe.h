#ifndef PIPE_H
#define PIPE_H

#include <stdint.h>

#define PIPE_COUNT     16
#define PIPE_BUF_SIZE  4096

void pipe_init(void);
int  pipe_create(void);                                   /* ترجع pipe id أو -1 */
int  pipe_write(int id, const void* data, uint32_t len);   /* ترجع عدد البايتات المكتوبة */
int  pipe_read(int id, void* data, uint32_t len);          /* ترجع عدد البايتات المقروءة (يمكن 0) */


/* --- API لطبقة التوافق (pipe(2) حقيقية: fds + EOF + EPIPE بدون SIGPIPE) --- */
void pipe_ref(int id, int is_writer);
void pipe_unref(int id, int is_writer);             /* تُحرَّر الـ slot عندما readers==writers==0 */
int  pipe_read_ex(int id, void* data, uint32_t len);        /* >0 بايتات، 0 = EOF، -1 = يجب الانتظار */
int  pipe_write_ex(int id, const void* data, uint32_t len); /* >0 بايتات، -1 = ممتلئ (انتظر)، -2 = لا قارئ (EPIPE) */

#endif
