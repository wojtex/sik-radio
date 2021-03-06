#include "shoutcast.hh"

#include <sstream>

#include <boost/regex.hpp>


ShoutCastClient::ShoutCastClient(std::shared_ptr<asio::TCPSocket> connection, std::string path,
                                 std::map<std::string, std::string> headers)
        : connection(connection),
          connection_closed(std::bind(&ShoutCastClient::on_disconnect, this)) {
    std::stringstream query;
    query << "GET " << path << " HTTP/1.0\r\n";
    for(auto &p : headers) {
        query << p.first << ": " << p.second << "\r\n";
    }
    query << "\r\n";

    std::string q = query.str();
    std::vector<asio::Byte> data(q.begin(), q.end());

    connection_established.action(std::bind(&ShoutCastClient::on_connect, this, std::move(data)));

    connection->on_connect.add(connection_established);
    connection->on_peer_shutdown.add(connection_closed);

    this->connection = connection;

    processor = new asio::Processor<asio::TCPSocket>(connection);
}

ShoutCastClient::~ShoutCastClient() {
    delete processor;
}



void ShoutCastClient::on_connect(std::vector<asio::Byte> query) {
    using namespace std::placeholders;
    connection->send(query);
    processor->task(new asio::tasks::ReadLineCRLF(std::bind(&ShoutCastClient::read_status_line, this, _1)));
}

void ShoutCastClient::on_disconnect() {
    connection.reset();
}

void ShoutCastClient::read_status_line(std::string status_line) {
    using namespace std::placeholders;
    std::stringstream line(status_line);
    line >> response_proto >> response_status;
    std::clog << status_line << std::endl;
    if(response_status != 200) {
        // TODO: error :(
    } else {
        // READ HEADERS
        processor->task(new asio::tasks::ReadLineCRLF(std::bind(&ShoutCastClient::read_headers, this, _1)));
    }
}

void ShoutCastClient::read_headers(std::string header_line) {
    using namespace std::placeholders;
    if(header_line == "") {
        // read content...
        std::clog << "---" << std::endl;
        if(response_headers.count("icy-metaint")) {
            this->metaint = std::stoi(response_headers["icy-metaint"]);
            processor->task(
                    new asio::tasks::ReadChunk(
                            this->metaint, std::bind(&ShoutCastClient::read_audio_data, this, _1)
                    )
            );
        } else {
            this->metaint = 4096;
            processor->task(
                    new asio::tasks::ReadChunk(
                            this->metaint, std::bind(&ShoutCastClient::read_audio_data_only, this, _1)
                    )
            );
        }

    } else {
        std::size_t colon = header_line.find_first_of(':');
        std::string key = header_line.substr(0, colon);
        if (header_line.size() > colon + 1 && header_line[colon + 1] == ' ') colon++;
        std::string value = header_line.substr(colon + 1);

        this->response_headers[key] = value;
        std::clog << header_line << std::endl;

        processor->task(new asio::tasks::ReadLineCRLF(std::bind(&ShoutCastClient::read_headers, this, _1)));
    }
}

void ShoutCastClient::skip_audio_data(std::vector<asio::Byte> data) {
    using namespace std::placeholders;

    // read metadata size
    processor->task(new asio::tasks::ReadByte(std::bind(&ShoutCastClient::read_metadata_header, this, _1)));
}

void ShoutCastClient::read_audio_data(std::vector<asio::Byte> data) {
    using namespace std::placeholders;
    if(!paused) {
        write(1, data.data(), data.size());
    }

    // read metadata size
    processor->task(new asio::tasks::ReadByte(std::bind(&ShoutCastClient::read_metadata_header, this, _1)));
}

void ShoutCastClient::read_audio_data_only(std::vector<asio::Byte> data) {
    using namespace std::placeholders;
    if(!paused) {
        write(1, data.data(), data.size());
    }

    // read next chunk
    processor->task(new asio::tasks::ReadChunk(this->metaint, std::bind(&ShoutCastClient::read_audio_data_only, this, _1)));
}

void ShoutCastClient::read_metadata_header(asio::Byte size) {
    using namespace std::placeholders;
    // read metadata chunk
    int rs = int(size) * 16;

    if(rs > 0) {
        processor->task(new asio::tasks::ReadChunk(rs, std::bind(&ShoutCastClient::read_metadata_data, this, _1)));
    } else {
        // read audio
        processor->task(new asio::tasks::ReadChunk(this->metaint, std::bind(&ShoutCastClient::read_audio_data, this, _1)));
    }
}

void ShoutCastClient::read_metadata_data(std::vector<asio::Byte> data) {
    using namespace std::placeholders;
    static const boost::regex re("([a-zA-Z0-9]+='[^']*';)*(StreamTitle='([^']*)';)([a-zA-Z0-9]+='[^']*';)*\\0*");
    // read the data
    // parse...
    std::string str(data.begin(), data.end());
    boost::smatch match;
    std::clog << str << std::endl;
    if(boost::regex_match(str, match, re)) {
        // got title
        std::clog << "Title: " << match[3] << std::endl;
        this->_title = match[3];
    }

    // read audio
    processor->task(new asio::tasks::ReadChunk(this->metaint, std::bind(&ShoutCastClient::read_audio_data, this, _1)));
}