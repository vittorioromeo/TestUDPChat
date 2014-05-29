// Copyright (c) 2013-2014 Vittorio Romeo
// License: Academic Free License ("AFL") v. 3.0
// AFL License page: http://opensource.org/licenses/AFL-3.0

#include <thread>
#include <future>
#include <SFML/Network.hpp>
#include <SSVUtils/SSVUtils.hpp>
#include <SSVUtilsJson/SSVUtilsJson.hpp>
#include <SSVStart/SSVStart.hpp>

using namespace std;
using namespace std::chrono;
using namespace ssvu;
using namespace ssvuj;
using namespace ssvs;

int choice(initializer_list<string> mChoices)
{
	lo("Choice") << endl;

	auto idx(0u);
	for(const auto& c : mChoices) { lo() << idx << ". " << c << endl; ++idx; }

	int result;
	while(true)
	{
		cin >> result;
		if(result < 0 || result >= static_cast<int>(mChoices.size())) { lo() << "Choice invalid, retry" << endl; continue; }
		return result;
	}
}
string strEnter() { lo("Enter string") << endl; string result; cin >> result; return result; }

// ---

using PTType = int;
using Uid = unsigned int;
using Port = unsigned short;

bool verbose{true};

enum PT : PTType{FromServer, FromClient};
enum PTFromServer : PTType{Accept, FSMessage};
enum PTFromClient : PTType{Connect, Ping, FCMessage};

inline sf::Packet& operator<<(sf::Packet& mPacket, const PT& mPT)				{ return mPacket << PTType(mPT); }
inline sf::Packet& operator>>(sf::Packet& mPacket, PT& mPT)						{ return mPacket >> reinterpret_cast<PTType&>(mPT); }
inline sf::Packet& operator<<(sf::Packet& mPacket, const PTFromServer& mPT)		{ return mPacket << PTType(mPT); }
inline sf::Packet& operator>>(sf::Packet& mPacket, PTFromServer& mPT)			{ return mPacket >> reinterpret_cast<PTType&>(mPT); }
inline sf::Packet& operator<<(sf::Packet& mPacket, const PTFromClient& mPT)		{ return mPacket << PTType(mPT); }
inline sf::Packet& operator>>(sf::Packet& mPacket, PTFromClient& mPT)			{ return mPacket >> reinterpret_cast<PTType&>(mPT); }

namespace Internal
{
	template<typename T> inline void appendToPacket(sf::Packet& mPacket, T&& mArg) { mPacket << std::forward<T>(mArg); }
	template<typename T, typename... TArgs> inline void appendToPacket(sf::Packet& mPacket, T&& mArg, TArgs&&... mArgs) { appendToPacket(mPacket, std::forward<T>(mArg)); appendToPacket(mPacket, std::forward<TArgs>(mArgs)...); }
	template<PTType TP, typename... TArgs> inline sf::Packet buildPacket(TArgs&&... mArgs) { sf::Packet result; appendToPacket(result, TP, std::forward<TArgs>(mArgs)...); return result; }
}

template<PTFromServer TP, typename... TArgs> inline sf::Packet buildPacketFromServer(TArgs&&... mArgs)	{ return ::Internal::buildPacket<PT::FromServer>(TP, std::forward<TArgs>(mArgs)...); }
template<PTFromClient TP, typename... TArgs> inline sf::Packet buildPacketFromClient(TArgs&&... mArgs)	{ return ::Internal::buildPacket<PT::FromClient>(TP, std::forward<TArgs>(mArgs)...); }

template<typename T> class PacketHandler
{
	private:
		using HandlerFunc = ssvu::Func<void(T&, sf::Packet&)>;
		std::unordered_map<PTType, HandlerFunc> funcs;

	public:
		inline void handle(PTType mType, T& mCaller, sf::Packet& mPacket)
		{
			try
			{
				auto itr(funcs.find(mType));
				if(itr == std::end(funcs))
				{
					if(verbose) lo("PacketHandler") << "Can't handle packet of type: " << mType << endl;
					return;
				}
				itr->second(mCaller, mPacket);
			}
			catch(std::exception& mException)
			{
				lo("PacketHandler") << "Exception during packet handling: (" << mType << ")" << endl << mException.what() << endl;
			}
			catch(...)
			{
				lo("PacketHandler") << "Unknown exception during packet handling: (" << mType << ")" << endl;
			}
		}

		HandlerFunc& operator[](PTType mType) noexcept { return funcs[mType]; }
};

struct Server;

class ClientHandler
{
	// A ClientHandler is a child object of a Server which deals with a specific Client
	// It can be attached to a Client (has an accepted Client) or not (free and ready to accept a Client)

	private:
		static constexpr int timeoutMax{5};
		Server& server;
		Uid uid;
		sf::UdpSocket& socket;
		PacketHandler<ClientHandler>& packetHandler;
		sf::IpAddress clientIp; Port clientPort;
		std::future<void> runFuture;
		bool attachedToClient{false};
		int timeoutUntil{timeoutMax};

	public:
		ClientHandler(Server& mServer, Uid mUid, sf::UdpSocket& mSocket, PacketHandler<ClientHandler>& mPacketHandler) : server(mServer), uid(mUid), socket(mSocket), packetHandler(mPacketHandler) { }

		inline void accept(const sf::IpAddress& mClientIp, Port mClientPort)
		{
			// Accepts a Client: sets attachedToClient to true and starts a new thread

			clientIp = mClientIp; clientPort = mClientPort; attachedToClient = true;
			runFuture = std::async(std::launch::async, [this]
			{
				while(attachedToClient)
				{
					if(--timeoutUntil <= 0)
					{
						attachedToClient = false;
						lo("ClientHandler #" + toStr(uid)) << "Timed out" << endl;
					}

					this_thread::sleep_for(chrono::seconds(1));
				}
			});
		}
		inline void refreshTimeout() noexcept { timeoutUntil = timeoutMax; }
		inline void handle(PTFromClient mType, sf::Packet& mPacket)
		{
			// Handles a packet from the Client

			refreshTimeout();
			if(mType != PTFromClient::Ping) packetHandler.handle(mType, *this, mPacket);
		}
		inline void sendToClient(sf::Packet mPacket)
		{
			// Sends a packet to the Client

			if(socket.send(mPacket, clientIp, clientPort) != sf::Socket::Done)
				lo("ClientHandler #" + toStr(uid)) << "Error sending" << endl;
		}

		inline bool isAttachedToClient() const noexcept		{ return attachedToClient; }
		inline Uid getUid() const noexcept					{ return uid; }
		inline Server& getServer() noexcept					{ return server; }
};

struct Client
{
	PacketHandler<Client>& packetHandler;
	sf::IpAddress serverIp; Port serverPort;
	sf::UdpSocket socket;
	std::future<void> runFuture;

	bool accepted{false}, busy{false};
	Uid uid;
	float pingTime{0.f};

	Client(PacketHandler<Client>& mPacketHandler, const sf::IpAddress& mServerIp, Port mServerPort) : packetHandler(mPacketHandler), serverIp(mServerIp), serverPort(mServerPort)
	{
		if(socket.bind(serverPort) != sf::Socket::Done) { lo("Client") << "Error binding socket to port: " << serverPort << endl; /* return; ? */ }
		socket.setBlocking(false);

		busy = true;
		runFuture = std::async(std::launch::async, [this]
		{
			lo("Client") << "Ip: " << serverIp << " || port: " << serverPort << " - trying to connect..." << endl;

			while(busy)
			{
				if(!accepted)
				{
					send(buildPacketFromClient<PTFromClient::Connect>());
					this_thread::sleep_for(chrono::seconds(1));
				}

				if(--pingTime <= 0.f)
				{
					send(buildPacketFromClient<PTFromClient::Ping>(uid));
					pingTime = 2000.f;
				}

				sf::Packet senderPacket; sf::IpAddress senderIp; Port senderPort;
				if(socket.receive(senderPacket, senderIp, senderPort) == sf::Socket::Done)
				{
					if(senderIp == serverIp && senderPort == serverPort)
					{
						if(verbose) lo("Client") << "Received packet from " << senderIp << " on port " << senderPort << endl;

						PT from; senderPacket >> from;
						if(from != PT::FromServer)
						{
							if(verbose) lo("Client") << "Packet from " << senderIp << " on port " << senderPort << " not from server, ignoring" << endl;
						}
						else
						{
							PTFromServer type; senderPacket >> type;
							if(!accepted && type == PTFromServer::Accept) connectionRequestAccepted(senderPacket);
							else if(accepted) packetHandler.handle(type, *this, senderPacket);
						}
					}
					else
					{
						if(verbose) lo("Client") << "Received packet, but not from server" << endl;
					}
				}

				this_thread::sleep_for(chrono::milliseconds(1));
			}
		});
	}
	~Client() { busy = false; /* runFuture.get(); ? */ }

	inline void connectionRequestAccepted(sf::Packet mPacket)
	{
		accepted = true; mPacket >> uid;
		lo("Client") << "Connected to server! Uid: " << uid << endl;
	}

	inline void send(sf::Packet mPacket) { if(socket.send(mPacket, serverIp, serverPort) != sf::Socket::Done) lo("Client") << "Error sending" << endl; }
};

struct Server
{
	PacketHandler<ClientHandler>& packetHandler;
	ssvu::VecUptr<ClientHandler> clientHandlers;
	sf::UdpSocket socket;
	Port port;
	Uid lastUid{0};
	std::future<void> runFuture;
	bool busy{false};

	Server(PacketHandler<ClientHandler>& mPacketHandler, Port mPort) : packetHandler(mPacketHandler), port(mPort)
	{
		if(socket.bind(port) != sf::Socket::Done) { lo("Server") << "Error binding socket to port: " << port << endl; return; }
		socket.setBlocking(false);

		busy = true;
		runFuture = std::async(std::launch::async, [this]
		{
			lo("Server") << "Starting on port: " << port << endl;

			while(busy)
			{
				sf::Packet clientPacket; sf::IpAddress clientIp; Port clientPort;
				if(socket.receive(clientPacket, clientIp, clientPort) == sf::Socket::Done)
				{
					if(verbose) lo("Server") << "Received packet from " << clientIp << " on port " << clientPort << endl;

					PT from; clientPacket >> from;
					if(from != PT::FromClient)
					{
						if(verbose) lo("Server") << "Packet from " << clientIp << " on port " << clientPort << " not from client, ignoring" << endl;
					}
					else
					{
						PTFromClient type; clientPacket >> type;
						if(verbose) lo("Server") << "...packet type " << type << endl;
						if(type == PTFromClient::Connect) acceptConnection(clientIp, clientPort);
						else
						{
							Uid chUid; clientPacket >> chUid;
							makeClientHandlerHandle(chUid, type, clientPacket);
						}
					}
				}

				this_thread::sleep_for(chrono::milliseconds(1));
			}
		});
	}
	~Server() { busy = false; }

	inline void grow()
	{
		lo("Server") << "Creating new client handlers" << endl;
		for(int i{0}; i < 10; ++i) ssvu::getEmplaceUptr<ClientHandler>(clientHandlers, *this, lastUid++, socket, packetHandler);
	}

	inline void acceptConnection(const sf::IpAddress& mClientIp, Port mClientPort)
	{
		bool foundNotBusy{false};

		for(auto& c : clientHandlers)
		{
			if(c->isAttachedToClient()) continue;
			foundNotBusy = true;

			sf::Packet acceptPacket{buildPacketFromServer<PTFromServer::Accept>(c->getUid())};
			if(socket.send(acceptPacket, mClientIp, mClientPort) != sf::Socket::Done)
			{
				lo("Server") << "Error sending accept packet" << endl;
			}
			else
			{
				lo("Server") << "Accepted client (" << c->getUid() << ")" << endl;
				c->accept(mClientIp, mClientPort); c->refreshTimeout();
				break;
			}
		}

		if(!foundNotBusy) grow();
	}

	inline void makeClientHandlerHandle(Uid mUid, PTFromClient mType, sf::Packet mPacket)
	{
		if(mUid >= clientHandlers.size())
		{
			if(verbose) lo("Server") << "Tried to make ClientHandler #" << mUid << " handle packet of type " << mType << " but it does not exist " << endl;
		}
		else if(!clientHandlers[mUid]->isAttachedToClient())
		{
			if(verbose) lo("Server") << "Tried to make ClientHandler #" << mUid << " handle packet of type " << mType << " but it's not busy " << endl;
		}
		else clientHandlers[mUid]->handle(mType, mPacket);
	}
	inline void makeAllClientHandlersHandle(PTFromClient mType, sf::Packet mPacket)
	{
		for(auto& c : clientHandlers) if(c->isAttachedToClient()) c->handle(mType, mPacket);
	}
	inline void sendToAllClients(sf::Packet mPacket)
	{
		for(auto& c : clientHandlers) if(c->isAttachedToClient()) c->sendToClient(mPacket);
	}
};

// ---

int main()
{
	PacketHandler<ClientHandler> sph;
	sph[PTFromClient::FCMessage] = [](ClientHandler& mCH, sf::Packet& mP)
	{
		string message; mP >> message;
		sf::Packet msgPacket{buildPacketFromServer<PTFromServer::FSMessage>(mCH.getUid(), message)};
		mCH.getServer().sendToAllClients(msgPacket);
	};

	PacketHandler<Client> cph;
	cph[PTFromServer::FSMessage] = [](Client&, sf::Packet& mP)
	{
		Uid uid; string message;
		mP >> uid >> message;
		lo("Chat message from #" + toStr(uid)) << message << endl;
	};

	/*Server s(sph, 27015);
	this_thread::sleep_for(chrono::milliseconds(100));
	Client c(cph, "127.0.0.1", 27015);

	while(true)
	{
		string input;
		if(std::getline(std::cin, input))
		{
			if(input == "exit") break;
			Packet clientMsg{buildPacketFromClient<PTFromClient::FCMessage>(c.uid, input)};
			c.send(clientMsg);
		}

		this_thread::sleep_for(chrono::milliseconds(1));
	}
	return 0;*/

	lo() << "Welcome to the test UDP chat." << endl;
	lo() << "Are you server or client?" << endl;

	switch(choice({"Server", "Client", "Exit"}))
	{
		case 0:
		{
			lo() << "What port?" << endl;
			int port{std::stoi(strEnter())};

			Server s(sph, port);

			while(true)
			{
				this_thread::sleep_for(chrono::milliseconds(1));
			}

			break;
		}
		case 1:
		{
			lo() << "What ip?" << endl;
			string ip{strEnter()};

			lo() << "What port?" << endl;
			int port{std::stoi(strEnter())};

			Client c(cph, ip, port);

			while(true)
			{
				string input;
				if(std::getline(std::cin, input))
				{
					sf::Packet clientMsg{buildPacketFromClient<PTFromClient::FCMessage>(c.uid, input)};
					c.send(clientMsg);
				}

				this_thread::sleep_for(chrono::milliseconds(1));
			}

			break;
		}
		case 2: return 0;
	}

	return 0;
}

// Multiple clients not working
// What port should the client listen on?
