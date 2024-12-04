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
class ChatSession {
public:
    ChatSession(tcp::socket socket, ChatRoom& room, int room_id, int user_id)
        : socket_(std::move(socket)), room_(room), room_id_(room_id), user_id_(user_id) {
    }

    void start(std::shared_ptr<ChatSession> self) {
        self_ = self;  // 자신을 관리하는 shared_ptr 저장
        room_.join(self);  // self 전달
        do_read();
    }

    tcp::socket& get_socket() { return socket_; }

private:
    void do_read() {
        boost::asio::async_read_until(socket_, boost::asio::dynamic_buffer(buffer_), "\n",
            [this](boost::system::error_code ec, std::size_t length) {
                if (!ec) {
                    std::string message = buffer_.substr(0, length - 1);
                    buffer_.erase(0, length);
                    room_.broadcast(message);
                    room_.save_message_to_db(room_id_, user_id_, message);
                    do_read();
                }
                else {
                    room_.leave(self_);  // shared_ptr를 직접 전달
                }
            });
    }

    tcp::socket socket_;
    ChatRoom& room_;
    int room_id_;
    int user_id_;
    std::string buffer_;
    std::shared_ptr<ChatSession> self_;  // 자신을 관리하는 shared_ptr
};

// 채팅 서버 클래스
class ChatServer {
public:
    ChatServer(boost::asio::io_context& io_context, const tcp::endpoint& endpoint)
        : acceptor_(io_context, endpoint) {
        initialize_database(); // 서버 시작 시 데이터베이스 초기화
        do_accept();
    }

private:
    void do_accept() {
        acceptor_.async_accept(
            [this](boost::system::error_code ec, tcp::socket socket) {
                if (!ec) {
                    std::cout << "New connection from " << socket.remote_endpoint() << std::endl;
                    int room_id = 1;  // 예제용 고정 ID
                    int user_id = 1;  // 예제용 고정 사용자 ID
                    auto session = std::make_shared<ChatSession>(std::move(socket), room_, room_id, user_id);
                    session->start(session);  // shared_ptr 전달
                }
                do_accept();
            });
    }

    tcp::acceptor acceptor_;
    ChatRoom room_;
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
