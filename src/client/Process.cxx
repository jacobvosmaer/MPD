/*
 * Copyright 2003-2019 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "Internal.hxx"
#include "Domain.hxx"
#include "protocol/Result.hxx"
#include "command/AllCommands.hxx"
#include "Log.hxx"
#include "util/StringAPI.hxx"
#include "util/CharUtil.hxx"

#define CLIENT_LIST_MODE_BEGIN "command_list_begin"
#define CLIENT_LIST_OK_MODE_BEGIN "command_list_ok_begin"
#define CLIENT_LIST_MODE_END "command_list_end"

inline CommandResult
Client::ProcessCommandList(bool list_ok,
			   std::list<std::string> &&list) noexcept
{
	CommandResult ret = CommandResult::OK;
	unsigned n = 0;

	for (auto &&i : list) {
		char *cmd = &*i.begin();

		FormatDebug(client_domain, "process command \"%s\"", cmd);
		ret = command_process(*this, n++, cmd);
		FormatDebug(client_domain, "command returned %i", int(ret));
		if (ret != CommandResult::OK || IsExpired())
			break;
		else if (list_ok)
			Write("list_OK\n");
	}

	return ret;
}

CommandResult
Client::ProcessLine(char *line) noexcept
{
	CommandResult ret;

	if (StringIsEqual(line, "noidle")) {
		if (idle_waiting) {
			/* send empty idle response and leave idle mode */
			idle_waiting = false;
			command_success(*this);
		}

		/* do nothing if the client wasn't idling: the client
		   has already received the full idle response from
		   client_idle_notify(), which he can now evaluate */

		return CommandResult::OK;
	} else if (idle_waiting) {
		/* during idle mode, clients must not send anything
		   except "noidle" */
		FormatWarning(client_domain,
			      "[%u] command \"%s\" during idle",
			      num, line);
		return CommandResult::CLOSE;
	}

	if (cmd_list.IsActive()) {
		if (StringIsEqual(line, CLIENT_LIST_MODE_END)) {
			FormatDebug(client_domain,
				    "[%u] process command list",
				    num);

			auto &&list = cmd_list.Commit();

			ret = ProcessCommandList(cmd_list.IsOKMode(),
						 std::move(list));
			FormatDebug(client_domain,
				    "[%u] process command "
				    "list returned %i", num, int(ret));

			if (ret == CommandResult::CLOSE ||
			    IsExpired())
				return CommandResult::CLOSE;

			if (ret == CommandResult::OK)
				command_success(*this);

			cmd_list.Reset();
		} else {
			if (!cmd_list.Add(line)) {
				FormatWarning(client_domain,
					      "[%u] command list size "
					      "is larger than the max (%lu)",
					      num,
					      (unsigned long)client_max_command_list_size);
				return CommandResult::CLOSE;
			}

			ret = CommandResult::OK;
		}
	} else {
		if (StringIsEqual(line, CLIENT_LIST_MODE_BEGIN)) {
			cmd_list.Begin(false);
			ret = CommandResult::OK;
		} else if (StringIsEqual(line, CLIENT_LIST_OK_MODE_BEGIN)) {
			cmd_list.Begin(true);
			ret = CommandResult::OK;
		} else if (IsUpperAlphaASCII(*line)) {
			/* no valid MPD command begins with an upper
			   case letter; this could be a badly routed
			   HTTP request */
			FormatWarning(client_domain,
				      "[%u] malformed command \"%s\"",
				      num, line);
			ret = CommandResult::CLOSE;
		} else {
			FormatDebug(client_domain,
				    "[%u] process command \"%s\"",
				    num, line);
			ret = command_process(*this, 0, line);
			FormatDebug(client_domain,
				    "[%u] command returned %i",
				    num, int(ret));

			if (ret == CommandResult::CLOSE || IsExpired())
				return CommandResult::CLOSE;

			if (ret == CommandResult::OK)
				command_success(*this);
		}
	}

	return ret;
}
