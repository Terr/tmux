/* $Id$ */

/*
 * Copyright (c) 2009 Tiago Cunha <me@tiagocunha.org>
 * Copyright (c) 2009 Nicholas Marriott <nicm@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/wait.h>

#include <stdlib.h>
#include <string.h>

#include "tmux.h"

/*
 * Runs a command without a window.
 */

void		 cmd_run_shell_prepare(struct cmd *, struct cmd_ctx *);
enum cmd_retval	 cmd_run_shell_exec(struct cmd *, struct cmd_ctx *);

void	cmd_run_shell_callback(struct job *);
void	cmd_run_shell_free(void *);
void	cmd_run_shell_print(struct job *, const char *);

const struct cmd_entry cmd_run_shell_entry = {
	"run-shell", "run",
	"t:", 1, 1,
	CMD_TARGET_PANE_USAGE " command",
	0,
	NULL,
	NULL,
	cmd_run_shell_exec,
	cmd_run_shell_prepare
};

struct cmd_run_shell_data {
	char		*cmd;
	struct cmd_ctx	 ctx;
	u_int		 wp_id;
};

void
cmd_run_shell_print(struct job *job, const char *msg)
{
	struct cmd_run_shell_data	*cdata = job->data;
	struct cmd_ctx			*ctx = &cdata->ctx;
	struct window_pane		*wp;

	wp = window_pane_find_by_id(cdata->wp_id);
	if (wp == NULL) {
		ctx->print(ctx, "%s", msg);
		return;
	}

	if (window_pane_set_mode(wp, &window_copy_mode) == 0)
		window_copy_init_for_output(wp);
	if (wp->mode == &window_copy_mode)
		window_copy_add(wp, "%s", msg);
}

void
cmd_run_shell_prepare(struct cmd *self, struct cmd_ctx *ctx)
{
	struct session	*s;
	struct client	*c;
	u_int		 i;

	if (args_has(self->args, 't')) {
		s = cmd_find_session(ctx, args_get(self->args, 't'), 0);
		ctx->ctx_s = s;
	}

	if (s != NULL) {
		for (i = 0; i < ARRAY_LENGTH(&clients); i++)
		{
			c = ARRAY_ITEM(&clients, i);
			if (c == NULL || c->session != s)
				continue;
		}
		ctx->ctx_c = c;
		ctx->ctx_wl = s->curw;
	}
}

enum cmd_retval
cmd_run_shell_exec(struct cmd *self, struct cmd_ctx *ctx)
{
	struct args			*args = self->args;
	struct cmd_run_shell_data	*cdata;
	struct format_tree		*ft;
	const char			*shellcmd;
	char				*shellcmd_run;
	struct window_pane		*wp = NULL;

	if (args_has(args, 't'))
		if (cmd_find_pane(ctx, args_get(args, 't'), NULL, &wp) == NULL)
			return (CMD_RETURN_ERROR);

	ft = format_create();
	cdata = xmalloc(sizeof *cdata);

	/* We can be called outside of any previous context. */
	if (wp != NULL)
		cdata->wp_id = wp->id;
	memcpy(&cdata->ctx, ctx, sizeof cdata->ctx);

	shellcmd = args->argv[0];

	if (ctx->cmdclient != NULL)
		ctx->cmdclient->references++;
	if (ctx->curclient != NULL)
		ctx->curclient->references++;

	if (ctx->ctx_s != NULL)
		format_session(ft, ctx->ctx_s);
	if (ctx->ctx_c != NULL)
		format_client(ft, ctx->ctx_c);
	if (ctx->ctx_wl != NULL && ctx->ctx_s != NULL)
		format_winlink(ft, ctx->ctx_s, ctx->ctx_wl);

	shellcmd_run = format_expand(ft, shellcmd);

	cdata->cmd = xstrdup(shellcmd_run);
	job_run(shellcmd_run, cmd_run_shell_callback, cmd_run_shell_free, cdata);

	format_free(ft);

	return (CMD_RETURN_YIELD);	/* don't let client exit */
}

void
cmd_run_shell_callback(struct job *job)
{
	struct cmd_run_shell_data	*cdata = job->data;
	struct cmd_ctx			*ctx = &cdata->ctx;
	char				*cmd, *msg, *line;
	size_t				 size;
	int				 retcode;
	u_int				 lines;

	if (ctx->cmdclient != NULL && ctx->cmdclient->flags & CLIENT_DEAD)
		return;
	if (ctx->curclient != NULL && ctx->curclient->flags & CLIENT_DEAD)
		return;

	lines = 0;
	do {
		if ((line = evbuffer_readline(job->event->input)) != NULL) {
			cmd_run_shell_print (job, line);
			lines++;
		}
	} while (line != NULL);

	size = EVBUFFER_LENGTH(job->event->input);
	if (size != 0) {
		line = xmalloc(size + 1);
		memcpy(line, EVBUFFER_DATA(job->event->input), size);
		line[size] = '\0';

		cmd_run_shell_print(job, line);
		lines++;

		free(line);
	}

	cmd = cdata->cmd;

	msg = NULL;
	if (WIFEXITED(job->status)) {
		if ((retcode = WEXITSTATUS(job->status)) != 0)
			xasprintf(&msg, "'%s' returned %d", cmd, retcode);
	} else if (WIFSIGNALED(job->status)) {
		retcode = WTERMSIG(job->status);
		xasprintf(&msg, "'%s' terminated by signal %d", cmd, retcode);
	}
	if (msg != NULL) {
		if (lines == 0)
			ctx->info(ctx, "%s", msg);
		else
			cmd_run_shell_print(job, msg);
		free(msg);
	}
}

void
cmd_run_shell_free(void *data)
{
	struct cmd_run_shell_data	*cdata = data;
	struct cmd_ctx			*ctx = &cdata->ctx;

	if (ctx->cmdclient != NULL) {
		ctx->cmdclient->references--;
		ctx->cmdclient->flags |= CLIENT_EXIT;
	}
	if (ctx->curclient != NULL)
		ctx->curclient->references--;

	free(cdata->cmd);
	free(cdata);
}
