#include <boost/asio.hpp>
#include <boost/bind/bind.hpp>
#include <sqlite3.h>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <memory>
#include <vector>
#include <fstream>

using boost::asio::ip::tcp;

// 데이터베이스 파일 경로
const std::string DB_FILE = "data/chat_server.db";

// SQLite 데이터베이스 초기화 함수
void initialize_database() {
    sqlite3* db;
    int rc = sqlite3_open(DB_FILE.c_str(), &db);
    if (rc) {
        std::cerr << "Can't open database: " << sqlite3_errmsg(db) << std::endl;
        return;
    }

    // users 테이블 생성
    const char* users_table = "CREATE TABLE users("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "login_id TEXT NOT NULL UNIQUE,"
        "login_password TEXT NOT NULL,"
        "name TEXT NOT NULL UNIQUE"
        ");";
    rc = sqlite3_exec(db, users_table, nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "Failed to create users table: " << sqlite3_errmsg(db) << std::endl;
    }

    // rooms 테이블 생성
    const char* rooms_table = "CREATE TABLE rooms("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "title TEXT NOT NULL UNIQUE,"
        "host_user_id INTEGER NOT NULL,"
        "FOREIGN KEY(host_user_id) REFERENCES users(id)"
        ");";
    rc = sqlite3_exec(db, rooms_table, nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "Failed to create rooms table: " << sqlite3_errmsg(db) << std::endl;
    }

    // talks 테이블 생성
    const char* talks_table = "CREATE TABLE talks ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "room_id INTEGER NOT NULL,"
        "user_id INTEGER NOT NULL,"
        "text TEXT NOT NULL,"
        "published_date TEXT NOT NULL,"
        "FOREIGN KEY(room_id) REFERENCES rooms(id),"
        "FOREIGN KEY(user_id) REFERENCES users(id)"
        ");";
    rc = sqlite3_exec(db, talks_table, nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "Failed to create talks table: " << sqlite3_errmsg(db) << std::endl;
    }

    sqlite3_close(db);
}

// 채팅방 클래스
class ChatRoom {
public:
    void join(std::shared_ptr<ChatSession> session) {
        members_.insert(session);
        broadcast("A new user has joined the chat.");
    }

    void leave(std::shared_ptr<ChatSession> session) {
        members_.erase(session);
        broadcast("A user has left the chat.");
    }

    void broadcast(const std::string& message) {
        for (auto& member : members_) {
            boost::asio::write(member->get_socket(), boost::asio::buffer(message + "\n"));
        }
    }

    void save_message_to_db(int room_id, int user_id, const std::string& message) {
        sqlite3* db;
        int rc = sqlite3_open(DB_FILE.c_str(), &db);
        if (rc) {
            std::cerr << "Can't open database: " << sqlite3_errmsg(db) << std::endl;
            return;
        }

        std::string sql = "INSERT INTO talks (room_id, user_id, text, published_date) VALUES (?, ?, ?, datetime('now'));";
        sqlite3_stmt* stmt;
        rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            std::cerr << "Failed to prepare statement: " << sqlite3_errmsg(db) << std::endl;
            sqlite3_close(db);
            return;
        }

        sqlite3_bind_int(stmt, 1, room_id);
        sqlite3_bind_int(stmt, 2, user_id);
        sqlite3_bind_text(stmt, 3, message.c_str(), -1, SQLITE_STATIC);

        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            std::cerr << "Failed to insert message: " << sqlite3_errmsg(db) << std::endl;
        }

        sqlite3_finalize(stmt);
        sqlite3_close(db);
    }

private:
    std::unordered_set<std::shared_ptr<ChatSession>> members_;  // shared_ptr 관리
};

// 채팅 세션 클래스
class ChatSession : public std::enable_shared_from_this<ChatSession> {
public:
    ChatSession(tcp::socket socket, ChatServer& server)
        : socket_(std::move(socket)), server_(server) {
    }

    void start(std::shared_ptr<ChatSession> self) {
        self_ = self;
        read_initial_data();
    }

    tcp::socket& get_socket() { return socket_; }

private:
    void read_initial_data() {
        auto self = shared_from_this();
        boost::asio::async_read_until(socket_, boost::asio::dynamic_buffer(buffer_), "\n",
            [this, self](boost::system::error_code ec, std::size_t length) {
                if (!ec) {
                    std::string data = buffer_.substr(0, length - 1);
                    buffer_.erase(0, length);

                    // room_id와 user_id 파싱
                    parse_initial_data(data);

                    // ChatServer를 통해 적절한 방 찾기
                    ChatRoom& room = server_.get_or_create_room(room_id_);
                    room.join(self);

                    do_read();
                }
                else {
                    std::cerr << "Error reading initial data: " << ec.message() << std::endl;
                }
            });
    }

    void parse_initial_data(const std::string& data) {
        auto comma_pos = data.find(',');
        if (comma_pos != std::string::npos) {
            room_id_ = std::stoi(data.substr(0, comma_pos));
            user_id_ = std::stoi(data.substr(comma_pos + 1));
        }
        else {
            std::cerr << "Invalid data format: " << data << std::endl;
        }
    }

    void do_read() {
        auto self = shared_from_this();
        boost::asio::async_read_until(socket_, boost::asio::dynamic_buffer(buffer_), "\n",
            [this, self](boost::system::error_code ec, std::size_t length) {
                if (!ec) {
                    std::string message = buffer_.substr(0, length - 1);
                    buffer_.erase(0, length);
                    server_.get_or_create_room(room_id_).broadcast(message);
                    do_read();
                }
                else {
                    server_.get_or_create_room(room_id_).leave(self);
                }
            });
    }

    tcp::socket socket_;
    ChatServer& server_;
    int room_id_;
    int user_id_;
    std::string buffer_;
    std::shared_ptr<ChatSession> self_;
};


// 채팅 서버 클래스
class ChatServer {
public:
    ChatServer(boost::asio::io_context& io_context, const tcp::endpoint& endpoint)
        : acceptor_(io_context, endpoint) {
        initialize_database(); // 서버 시작 시 데이터베이스 초기화
        do_accept();
    }

    // 특정 room_id에 해당하는 ChatRoom을 반환 (없으면 생성)
    ChatRoom& get_or_create_room(int room_id) {
        auto it = rooms_.find(room_id);
        if (it == rooms_.end()) {
            auto emplace_result = rooms_.emplace(room_id, std::make_unique<ChatRoom>());
            auto& new_it = emplace_result.first;  // 삽입된 요소의 반복자
            bool success = emplace_result.second; // 삽입 성공 여부
            return *(new_it->second);
        }
        return *(it->second);
    }
private:
    void do_accept() {
        acceptor_.async_accept(
            [this](boost::system::error_code ec, tcp::socket socket) {
                if (!ec) {
                    std::cout << "New connection from " << socket.remote_endpoint() << std::endl;

                    auto session = std::make_shared<ChatSession>(std::move(socket), *this);
                    session->start(session);  // ChatSession에서 room_id와 user_id를 받아 방에 입장
                }
                do_accept();
            });
    }

    tcp::acceptor acceptor_;
    std::unordered_map<int, std::unique_ptr<ChatRoom>> rooms_; // room_id별 ChatRoom 관리
};


int main() {
    try {
        boost::asio::io_context io_context;

        tcp::endpoint endpoint(tcp::v4(), 12345); // 포트 12345에서 수신 대기
        ChatServer server(io_context, endpoint);

        std::cout << "Chat server is running on port 12345..." << std::endl;

        io_context.run();
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }

    return 0;
}
