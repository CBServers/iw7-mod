#include <std_include.hpp>
#include "../steam.hpp"

#include "component/friends.hpp"

namespace steam
{
	namespace
	{
		// 24-byte FriendGameInfo_t: game_id | game_ip | game_port | query_port | lobby steam_id
		struct friend_game_info
		{
			game_id game;
			unsigned int game_ip;
			unsigned short game_port;
			unsigned short query_port;
			steam_id lobby;
		};

		constexpr unsigned int IW7_APP_ID = 292730;

		// Lobby id shape the game validates before treating a friend as joinable (type 8, instance bit 0x40000)
		steam_id make_lobby_id(const unsigned int account_id)
		{
			steam_id id{};
			id.raw.account_id = account_id;
			id.raw.account_instance = 0x40000;
			id.raw.account_type = 8;
			id.raw.universe = 1;
			return id;
		}

		bool find_cb_friend(const steam_id id, ::friends::friend_record& out)
		{
			return ::friends::find_friend(id.bits, out);
		}
	}

	const char* friends::GetPersonaName()
	{
		return "1337";
	}

	unsigned long long friends::SetPersonaName(const char* pchPersonaName)
	{
		return 0;
	}

	int friends::GetPersonaState()
	{
		return 1;
	}

	int friends::GetFriendCount(int eFriendFlags)
	{
		return static_cast<int>(::friends::get_friends().size());
	}

	steam_id friends::GetFriendByIndex(int iFriend, int iFriendFlags)
	{
		const auto list = ::friends::get_friends();
		if (iFriend < 0 || iFriend >= static_cast<int>(list.size()))
		{
			return steam_id();
		}

		steam_id id{};
		id.bits = list[iFriend].steam_id_bits;
		return id;
	}

	int friends::GetFriendRelationship(steam_id steamIDFriend)
	{
		::friends::friend_record record{};
		return find_cb_friend(steamIDFriend, record) ? 3 : 0;
	}

	int friends::GetFriendPersonaState(steam_id steamIDFriend)
	{
		::friends::friend_record record{};
		return find_cb_friend(steamIDFriend, record) ? record.persona_state : 0;
	}

	const char* friends::GetFriendPersonaName(steam_id steamIDFriend)
	{
		::friends::friend_record record{};
		if (find_cb_friend(steamIDFriend, record))
		{
			static thread_local std::string name_buffer;
			name_buffer = record.name;
			return name_buffer.data();
		}

		return "";
	}

	bool friends::GetFriendGamePlayed(steam_id steamIDFriend, void* pFriendGameInfo)
	{
		::friends::friend_record record{};
		if (!find_cb_friend(steamIDFriend, record) || !record.in_game)
		{
			return false;
		}

		if (pFriendGameInfo)
		{
			auto* info = static_cast<friend_game_info*>(pFriendGameInfo);
			info->game.bits = 0;
			info->game.raw.app_id = IW7_APP_ID;
			info->game_ip = 0;
			info->game_port = 0;
			info->query_port = 0;
			info->lobby = make_lobby_id(steamIDFriend.raw.account_id);
		}

		return true;
	}

	const char* friends::GetFriendPersonaNameHistory(steam_id steamIDFriend, int iPersonaName)
	{
		return "";
	}

	bool friends::HasFriend(steam_id steamIDFriend, int eFriendFlags)
	{
		::friends::friend_record record{};
		return find_cb_friend(steamIDFriend, record);
	}

	int friends::GetClanCount()
	{
		return 0;
	}

	steam_id friends::GetClanByIndex(int iClan)
	{
		return steam_id();
	}

	const char* friends::GetClanName(steam_id steamIDClan)
	{
		return "3arc";
	}

	const char* friends::GetClanTag(steam_id steamIDClan)
	{
		return this->GetClanName(steamIDClan);
	}

	bool friends::GetClanActivityCounts(steam_id steamID, int* pnOnline, int* pnInGame, int* pnChatting)
	{
		return false;
	}

	unsigned long long friends::DownloadClanActivityCounts(steam_id groupIDs[], int nIds)
	{
		return 0;
	}

	int friends::GetFriendCountFromSource(steam_id steamIDSource)
	{
		return 0;
	}

	steam_id friends::GetFriendFromSourceByIndex(steam_id steamIDSource, int iFriend)
	{
		return steam_id();
	}

	bool friends::IsUserInSource(steam_id steamIDUser, steam_id steamIDSource)
	{
		return false;
	}

	void friends::SetInGameVoiceSpeaking(steam_id steamIDUser, bool bSpeaking)
	{
	}

	void friends::ActivateGameOverlay(const char* pchDialog)
	{
	}

	void friends::ActivateGameOverlayToUser(const char* pchDialog, steam_id steamID)
	{
	}

	void friends::ActivateGameOverlayToWebPage(const char* pchURL)
	{
	}

	void friends::ActivateGameOverlayToStore(unsigned int nAppID, unsigned int eFlag)
	{
	}

	void friends::SetPlayedWith(steam_id steamIDUserPlayedWith)
	{
	}

	void friends::ActivateGameOverlayInviteDialog(steam_id steamIDLobby)
	{
	}

	int friends::GetSmallFriendAvatar(steam_id steamIDFriend)
	{
		return 0;
	}

	int friends::GetMediumFriendAvatar(steam_id steamIDFriend)
	{
		return 0;
	}

	int friends::GetLargeFriendAvatar(steam_id steamIDFriend)
	{
		return 0;
	}

	bool friends::RequestUserInformation(steam_id steamIDUser, bool bRequireNameOnly)
	{
		return false;
	}

	unsigned long long friends::RequestClanOfficerList(steam_id steamIDClan)
	{
		return 0;
	}

	steam_id friends::GetClanOwner(steam_id steamIDClan)
	{
		return steam_id();
	}

	int friends::GetClanOfficerCount(steam_id steamIDClan)
	{
		return 0;
	}

	steam_id friends::GetClanOfficerByIndex(steam_id steamIDClan, int iOfficer)
	{
		return steam_id();
	}

	int friends::GetUserRestrictions()
	{
		return 0;
	}

	bool friends::SetRichPresence(const char* pchKey, const char* pchValue)
	{
		return true;
	}

	void friends::ClearRichPresence()
	{
	}

	const char* friends::GetFriendRichPresence(steam_id steamIDFriend, const char* pchKey)
	{
		// The native list shows the "status" key verbatim for a friend in this title.
		::friends::friend_record record{};
		if (!pchKey || std::string_view(pchKey) != "status" || !find_cb_friend(steamIDFriend, record))
		{
			return "";
		}

		static thread_local std::string status_buffer;
		status_buffer = ::friends::get_presence_text(record);
		return status_buffer.data();
	}

	int friends::GetFriendRichPresenceKeyCount(steam_id steamIDFriend)
	{
		return 0;
	}

	const char* friends::GetFriendRichPresenceKeyByIndex(steam_id steamIDFriend, int iKey)
	{
		return "a";
	}

	void friends::RequestFriendRichPresence(steam_id steamIDFriend)
	{
	}

	bool friends::InviteUserToGame(steam_id steamIDFriend, const char* pchConnectString)
	{
		return ::friends::request_invite(steamIDFriend.bits);
	}

	int friends::GetCoplayFriendCount()
	{
		return 0;
	}

	steam_id friends::GetCoplayFriend(int iCoplayFriend)
	{
		return steam_id();
	}

	int friends::GetFriendCoplayTime(steam_id steamIDFriend)
	{
		return 0;
	}

	unsigned int friends::GetFriendCoplayGame(steam_id steamIDFriend)
	{
		return 0;
	}

	unsigned long long friends::JoinClanChatRoom(steam_id steamIDClan)
	{
		return 0;
	}

	bool friends::LeaveClanChatRoom(steam_id steamIDClan)
	{
		return false;
	}

	int friends::GetClanChatMemberCount(steam_id steamIDClan)
	{
		return 0;
	}

	steam_id friends::GetChatMemberByIndex(steam_id steamIDClan, int iUser)
	{
		return steam_id();
	}

	bool friends::SendClanChatMessage(steam_id steamIDClanChat, const char* pchText)
	{
		return false;
	}

	int friends::GetClanChatMessage(steam_id steamIDClanChat, int iMessage, void* prgchText, int cchTextMax,
	                                unsigned int* peChatEntryType, steam_id* pSteamIDChatter)
	{
		return 0;
	}

	bool friends::IsClanChatAdmin(steam_id steamIDClanChat, steam_id steamIDUser)
	{
		return false;
	}

	bool friends::IsClanChatWindowOpenInSteam(steam_id steamIDClanChat)
	{
		return false;
	}

	bool friends::OpenClanChatWindowInSteam(steam_id steamIDClanChat)
	{
		return false;
	}

	bool friends::CloseClanChatWindowInSteam(steam_id steamIDClanChat)
	{
		return false;
	}

	bool friends::SetListenForFriendsMessages(bool bInterceptEnabled)
	{
		return false;
	}

	bool friends::ReplyToFriendMessage(steam_id steamIDFriend, const char* pchMsgToSend)
	{
		return false;
	}

	int friends::GetFriendMessage(steam_id steamIDFriend, int iMessageID, void* pvData, int cubData,
	                              unsigned int* peChatEntryType)
	{
		return 0;
	}

	unsigned long long friends::GetFollowerCount(steam_id steamID)
	{
		return 0;
	}

	unsigned long long friends::IsFollowing(steam_id steamID)
	{
		return 0;
	}

	unsigned long long friends::EnumerateFollowingList(unsigned int unStartIndex)
	{
		return 0;
	}
}
