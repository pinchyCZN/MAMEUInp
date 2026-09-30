defines {
	"UNICODE",
	"_UNICODE",
	"OSD_WINDOWS",
	"USE_SDL=0",
	--"WIN95_MULTIMON"
	"_WIN32_WINNT=0x0501",
	"USE_QTDEBUG=0",
	"WIN32_LEAN_AND_MEAN",
	"NOMINMAX",
}

configuration { "vs*" }
	flags {
		"Unicode",
	}
	buildoptions {
		"/WX-",   -- winui has never been built with MSVC; genie.lua sets /WX globally
	}

configuration { }

if not _OPTIONS["DONT_USE_NETWORK"] then
	defines {
		"USE_NETWORK",
		"OSD_NET_USE_PCAP",
	}
end

