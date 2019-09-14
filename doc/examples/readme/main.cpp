#include <azmq/socket.hpp>
#include <boost/asio.hpp>
#include <boost/program_options.hpp>
#include <array>
#include <iostream>
#include <type_traits>
#include <fstream>
#include <memory>
#include <filesystem>
#include <rapidjson/document.h>
#include <nlohmann/json.hpp>
#include <boost/bind.hpp>


namespace asio = boost::asio;
namespace options = boost::program_options;
#define PORT 7721


class FileReader
{
    typedef std::shared_ptr<asio::streambuf> StreambufPtr;
    typedef std::shared_ptr<FileReader> FileReaderPtr;
    typedef std::weak_ptr<FileReader> FileReaderWeakPtr;
public:
    static FileReaderWeakPtr Create(asio::io_service& io_service, const std::string& path);
    virtual ~FileReader();

    void HandleRead(FileReaderPtr me, StreambufPtr sb,
                    const boost::system::error_code &error);
private:
    FileReader(asio::io_service& io_service, const std::string& path);
    asio::posix::stream_descriptor m_InputStream;
};

FileReader::FileReaderWeakPtr FileReader::Create(asio::io_service& io_service,
                                                 const std::string& path){
    FileReaderPtr ptr(new FileReader(io_service, path));
    StreambufPtr sb(new boost::asio::streambuf());

    asio::async_read(ptr->m_InputStream, *sb,
                     boost::bind(&FileReader::HandleRead, ptr.get(),
                                 ptr, sb, asio::placeholders::error));
    return ptr;
}

FileReader::FileReader(asio::io_service& io_service, const std::string& path)
        :m_InputStream(io_service)
{
    int dev = open(path.c_str(), O_RDONLY);
    if (dev == -1) {
        throw std::runtime_error("failed to open device " + path);
    }

    m_InputStream.assign(dev);
}

void FileReader::HandleRead(FileReaderPtr me, StreambufPtr sb,
                            const boost::system::error_code &error) {
    if(!error) {
        //Inform all of a sucessfull read
        std::istream is(sb.get());
        size_t data_length = sb->size();
        asio::async_read(m_InputStream, *sb,
                         boost::bind(&FileReader::HandleRead, this, me, sb, asio::placeholders::error));
    }
}

// https://dakerfp.github.io/post/weak_ptr_singleton/
class AppIO {
private:
    AppIO(): _ioContext(std::make_shared<asio::io_context>(1))
    {
    }
    std::shared_ptr<asio::io_context> _ioContext;
    std::string _appName;
    std::string _appInstanceName;
    std::shared_ptr<nlohmann::json> _config;
    std::filesystem::path _cfgFile;

    AppIO(const AppIO&) = delete;
    void writeDefaultConfig(const std::filesystem::path& cfgFile) {
        auto dirPath = cfgFile.parent_path();
        if(!std::filesystem::exists(dirPath)) {
            std::filesystem::create_directories(dirPath);
        }

        std::ofstream outCfg(cfgFile.string());
        nlohmann::json j;
        std::time_t result = std::time(nullptr);
        j["_created"] = std::asctime(std::localtime(&result));
        outCfg << j << std::endl;
        outCfg.close();
    }

    nlohmann::json openConfig(const std::filesystem::path& cfgFile) {
        nlohmann::json configAsJson;
        std::ifstream inCfg(cfgFile.string());
        inCfg >> configAsJson;
        inCfg.close();
        return configAsJson;
    }

    void initializeConfig() {
        std::string home = getenv("HOME");
        _cfgFile = home + "/.industry/config/" + getAppNameAndInstance() + ".json";
        if (!std::filesystem::exists(_cfgFile)) {
            writeDefaultConfig(_cfgFile);
        }

        _config = std::make_shared<nlohmann::json>(openConfig(_cfgFile));
    }

public:
    ~AppIO() { }
    static std::shared_ptr<AppIO> instance() {
        static std::weak_ptr<AppIO> _instance;
        // Todo: support multithread with mutex lock
        if (auto ptr = _instance.lock()) { // .lock() returns a shared_ptr and increments the refcount
            return ptr;
        }
        auto ptr = std::shared_ptr<AppIO>(new AppIO());
        _instance = ptr;
        return ptr;
    }
    std::shared_ptr<asio::io_context> getContext() {
        return _ioContext;
    }
    const std::string& getAppName() {
        return _appName;
    }
    const std::string& getInstanceAppName() {
        return _appInstanceName;
    }
    const std::string getAppNameAndInstance() {
        return _appName+"/"+_appInstanceName;
    }
    std::shared_ptr<nlohmann::json> getConfig() {
        return _config;
    }
    void updateConfigFile() {
        asio::posix::stream_descriptor out(*_ioContext);
        auto path = _cfgFile.string();
        int dev = open(path.c_str(), O_RDONLY);
        if (dev == -1) throw std::runtime_error("failed to open device " + path);
        out.assign(dev);

        out.async_read_some()
//        auto reader = FileReader::Create(*_ioContext, _cfgFile.string());
    }
    void initialize(int argc, char** argv) {
        _appName = std::filesystem::path(argv[0]).filename();

        options::options_description desc{"Options"};
        desc.add_options()
                ("help,h", "Help screen")
                ("name,n", options::value<std::string>()->default_value("default"), "Application named used for configuration and data distribution topics");

        options::variables_map vm;
        options::store(parse_command_line(argc, argv, desc), vm);
        options::notify(vm);


        if (vm.count("help")) {
            std::cout << desc << '\n';
            exit(0);
        }
        _appInstanceName = vm["name"].as<std::string>();
        std::cout << "Starting app: " << _appName << "." << _appInstanceName << '\n';

        initializeConfig();
    }
};



template<class T>
class Receiver {
public:
    typedef std::function<T(const std::string&)> toT;
    Receiver(std::string name): _subscriber(*AppIO::instance()->getContext()) {
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
        auto myApp = AppIO::instance();
        _address = _typeName+"."+myApp->getAppName()+"."+myApp->getInstanceAppName()+"."+name;

        auto conf = myApp->getConfig();
        if (conf->find("_receivers") == conf->end()) {
            (*conf)["_receivers"] = {};
        }
        if ((*conf)["_receivers"].find(_address) == (*conf)["_receivers"].end()) {
            (*conf)["_receivers"][_address] = "";
        }
        auto connectedTo = (*conf)["_receivers"][_address];

        myApp->updateConfigFile();
        std::cout << "hahahahahahah";
    }
    Receiver(T initalState, std::string typeName, const toT & toTemplateVal): _subscriber(*AppIO::instance()->getContext()) {
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

    T getState() { return _lastState; }
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
    std::string _address;
};

template<class T>
class Sender {
public:
    typedef std::function<std::string(T)> TtoString;

    Sender(const std::string & topic): Sender() {
        setTopic(topic);
    }
    Sender(): _publisher(*AppIO::instance()->getContext()) {
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
        auto myApp = AppIO::instance();
        _topic = _typeName+"."+myApp->getAppNameAndInstance()+"."+topic;
    }
    void send(const T & val) {
        _lastState = val;
        auto strToSend = _topic+_toString(val);
        std::cout << "publishing " << strToSend << "\n";
        _publisher.send(asio::buffer(strToSend));
    }
    T getState() { return _lastState; }
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

    auto myApp = AppIO::instance();
    myApp->initialize(argc, argv);

    Receiver<bool> boolReceiver("whazza");
    boolReceiver.subscribeTo("bool.readme/default.NASDAQ");
    boolReceiver.setCallback([](bool state){
        std::cout << "\n\n\n state is " << state << "\n\n\n";
    });

    Sender<bool> boolSender("NASDAQ");


    asio::steady_timer t(*myApp->getContext(), boost::asio::chrono::seconds(1));

    auto onTimeout = [&boolSender](boost::system::error_code const& ec){
        std::cout << "got new time interval publishing data\n";
        boolSender.send(true);
    };

    t.async_wait(onTimeout);

    boost::asio::signal_set signals(*myApp->getContext(), SIGINT, SIGTERM);
    signals.async_wait( [myApp](auto, auto){
        myApp->getContext()->stop();
        std::cout << " deinitializing\n";
    });
    myApp->getContext()->run();

    return 0;
}