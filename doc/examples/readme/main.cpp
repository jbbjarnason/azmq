#include <azmq/socket.hpp>
#include <boost/asio.hpp>
#include <array>
#include <iostream>
#include <type_traits>
#include <experimental/filesystem>
#include <rapidjson/document.h>

namespace asio = boost::asio;
auto context = std::make_shared<asio::io_context>(1);

#define PORT 7721

template<class T>
class TransmissionBase {
public:
    T getState() { return _lastState; }
    T _lastState;
protected:
};


template<class T>
class Receiver {
public:
    typedef std::function<T(const std::string&)> toT;

    Receiver(): _subscriber(*context) {
        if (std::is_same<T, bool>::value) init(false, "bool", [this](const std::string & json){
            std::cout << "to json: " << json <<"\n";
            return parse(json)["val"].GetBool();
        });
        else if (std::is_same<T, int>::value) init(0, "int", [this](const std::string & json){
            return parse(json)["val"].GetInt();
        });
        else if (std::is_same<T, double>::value) init(0.0, "double", [this](const std::string & json){
            return parse(json)["val"].GetDouble();
        });
        else if (std::is_same<T, std::string>::value) init("", "string", [this](const std::string & json){
            return parse(json)["val"].GetString();
        });
        else throw "Unknown type in sender declared in default constructor";
    }
    Receiver(T initalState, std::string typeName, const toT & toTemplateVal): _subscriber(*context) {
        init(initalState, typeName, toTemplateVal);
    }

    void subscribeTo(const std::string & name) {
        _subscriber.set_option(azmq::socket::subscribe(name.c_str()));
        _subscribtionStringLen = name.length();
    }
    void setCallback(const std::function<void(T)> & cb){
        _subscriber.async_receive(asio::buffer(_buf), [this, cb](auto const& error_code, auto bytes_transferred){
           if (error_code) {
               std::cout << "Got an error " << error_code << "\n";
               return;
           }
            auto output = std::string(_buf.data(), bytes_transferred);
//            auto output = std::string(_buf.data(), bytes_transferred - 1);
            std::cout << "got new output " << output<< "\n" ; //<< " parsed is "<< _lastState << "\n";
            _lastState = _stringToTemplateVal(output.substr(_subscribtionStringLen, output.length()));

           cb(_lastState);
        });
    }

private:
    void init(T initialState, std::string typeName, const toT & toTemplateVal) {
        _lastState = initialState;
        _typeName = typeName;
        _stringToTemplateVal = toTemplateVal;
        _subscriber.connect("tcp://127.0.0.1:" + std::to_string(PORT));
    }
    inline rapidjson::Document parse(const std::string & json) {
        rapidjson::Document doc;
        doc.Parse(json.c_str());
        return doc;
    }

    toT _stringToTemplateVal;

    azmq::sub_socket _subscriber;
    std::array<char, 256> _buf{};
    std::string _typeName = "";
    uint _subscribtionStringLen;
    T _lastState;
};

template<class T>
class Sender {
public:
    typedef std::function<std::string(T)> TtoString;
    Sender(): _publisher(*context) {
        if (std::is_same<T, bool>::value) init(false, "bool", [this](const T & val){
            return "{\"val\":true}";
        });
        else if (std::is_same<T, int>::value) init(0, "int", [this](const T & val){
                return "{\"val\":true}";
            });
        else if (std::is_same<T, double>::value) init(0.0, "double", [this](const T & val){
                return "{\"val\":true}";
            });
        else if (std::is_same<T, std::string>::value) init("", "string", [this](const T & val){
                return "{\"val\":true}";
            });
        else throw "Unknown type in sender declared in default constructor";
    }
    void setTopic(const std::string & topic) {
        _topic = topic;
    }
    void send(const T & val) {
        auto strToSend = _topic+_toString(val);
        std::cout << "publishing " << strToSend << "\n";
        _publisher.send(asio::buffer(strToSend));
    }
private:
    void init(T initialState, std::string typeName, TtoString toString) {
        _lastState = initialState;
        _typeName = typeName;
        _toString = toString;
        _publisher.bind("tcp://127.0.0.1:" + std::to_string(PORT));
    }

    T _lastState;
    std::string _typeName = "";
    TtoString _toString;
    azmq::pub_socket _publisher;
    std::string _topic;
};



int main(int argc, char** argv) {
    asio::io_context io_context(1);

    std::cout << "appname: " << argv[0] << '\n';
//    std::cout << "current path is: " << std::experimental::filesystem::current_path() << '\n';


    Receiver<bool> boolReceiver;
    boolReceiver.subscribeTo("NASDAQ");
    boolReceiver.setCallback([](bool state){
        std::cout << "\n\n\n state is " << state << "\n\n\n";
    });



//    azmq::sub_socket subscriber(*context);
//    subscriber.connect("tcp://127.0.0.1:7721");
//    subscriber.set_option(azmq::socket::subscribe("NASDAQ"));

//    std::array<char, 256> buf_{};
//    subscriber.async_receive(asio::buffer(buf_), [&buf_](boost::system::error_code const& ec, size_t bytes_transferred) {
//        if (ec) return;
//        auto output = boost::string_ref(buf_.data(), bytes_transferred - 1);
//        std::cout << "got new async receive with data: " << output << "\n";
//    });

//    azmq::pub_socket publisher(*context);
//    publisher.bind("tcp://127.0.0.1:7721");

    Sender<bool> boolSender;
    boolSender.setTopic("NASDAQ");


    asio::steady_timer t(*context, boost::asio::chrono::seconds(1));

    auto onTimeout = [&boolSender](boost::system::error_code const& ec){
        std::cout << "got new time interval publishing data\n";
//        publisher.send(asio::buffer("NASDAQ{\"val\":true}"));
        boolSender.send(true);
    };

    t.async_wait(onTimeout);

    boost::asio::signal_set signals(*context, SIGINT, SIGTERM);
    signals.async_wait( [](auto, auto){
        context->stop();
        std::cout << " deinitializing\n";
    });
    context->run();

    return 0;
}