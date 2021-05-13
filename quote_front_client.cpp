#include <stdio.h>
#include <signal.h>
#include <memory.h>
#include <boost/lockfree/spsc_queue.hpp>
#include <boost/thread.hpp>
#include <boost/chrono.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/enable_shared_from_this.hpp>
#include <boost/shared_ptr.hpp>
#include <asio.hpp>
#include <asio/io_service.hpp>
#include <set>

#define PRINT_BUFF_COUNT 100 
#define BUF_SIZE 8000
#define CONNECT_PER_LOOP 200

unsigned int max_fds = 1;

int log_fd = -1;
char svr_ip[32] = "192.168.1.229";
int port = 11700;

boost::lockfree::spsc_queue<char*, 
        boost::lockfree::capacity<PRINT_BUFF_COUNT> , 
        boost::lockfree::fixed_sized<true> > print_buff;

boost::lockfree::spsc_queue<char*, 
        boost::lockfree::capacity<PRINT_BUFF_COUNT> , 
        boost::lockfree::fixed_sized<true> > print_data;

void handle_sig(int signum)
{
    printf("receiv sig int");
    exit(0);
}
class event_handler;

void print_handler()
{
	for (int i = 0; i < PRINT_BUFF_COUNT; i++)
	{
		print_buff.push(new char[BUF_SIZE]);
	}

	boost::thread th(
		[] {
		char *buf;
		while (true) {
			while (print_data.pop(buf))
			{
				if (*buf)
				{
					std::cout << "recv: " << buf << std::endl;;
				}
				print_buff.push(buf);
			}
			boost::this_thread::sleep(boost::posix_time::milliseconds(5));
		}
	});
	th.join();
}

class tcp_channel : public boost::enable_shared_from_this<tcp_channel>
{
public:
	tcp_channel(asio::io_service &io_):socket_(io_),ep(asio::ip::address::from_string(svr_ip), port)
	{
	}
	~tcp_channel()
	{
		socket_.close();
		std::cout << "channel release ..." << std::endl;
	}

	void start(std::function<void()> f, bool need_print = false)
	{
		this->need_print = need_print;
		exit_ = f;
		do_connect();
	}

	void do_connect()
	{
		auto self(shared_from_this());
		socket_.async_connect(ep,
			[self](std::error_code ec)
		{
			if (!ec)
			{
				self->do_write();
			}
			else
			{
				printf("do_accept failure: Errcode=%d,ErrMsg=%s\n", ec.value(), ec.message().c_str());
				self->exit_();
			}
		});
	}
		
private:
	void do_write()
	{
		auto self(shared_from_this());
		asio::async_write(socket_,
			asio::buffer(sub_msg.c_str(), sub_msg.length()),
			[self](std::error_code ec, std::size_t length)
		{
			if (!ec)
			{
				if(!self->need_print)
				{
					self->do_read();
				}
				else
				{
					self->do_read_print();
				}
			}
			else
			{
				printf("do_write Disconnect:PeerPort=%d,Errcode=%d,ErrMsg=%s\n", self->socket_.remote_endpoint().port(), ec.value(), ec.message().c_str());
				self->exit_();
			}
		});
	}
	
	void do_read()
	{
		auto self(shared_from_this());
		socket_.async_read_some(asio::buffer(buf, BUF_SIZE),
			[self](std::error_code ec, std::size_t bytes)
		{
			if (!ec)
			{
				self->do_read();
			}
			else
			{
				printf("do_read Disconnect:PeerPort=%d,Errcode=%d,ErrMsg=%s\n", self->socket_.remote_endpoint().port(), ec.value(), ec.message().c_str());
				self->exit_();
			}
		});
	}

	void do_read_print()
	{
		char *buff;
	    bool is_print = print_buff.pop(buff);
		if (is_print)
		{
			buff = buf;
			std::cout << "print buff exhaust " << std::endl;
		}
		auto self(shared_from_this());
		socket_.async_read_some(asio::buffer(buff, BUF_SIZE),
			[self, buff, is_print](std::error_code ec, std::size_t bytes)
		{
			if (!ec)
			{
				if (is_print)
				{
					print_data.push(buff);
				}
				self->do_read_print();
			}
			else
			{
				printf("do_read Disconnect:PeerPort=%d,Errcode=%d,ErrMsg=%s\n", self->socket_.remote_endpoint().port(), ec.value(), ec.message().c_str());
				self->exit_();
			}
		});
	}

	char buf[BUF_SIZE + 1];
	asio::ip::tcp::socket socket_;
	boost::function<void()> exit_;
	asio::ip::tcp::endpoint ep;
	bool need_print;

	std::string sub_msg{ "{(len=29)MARKET01@@S@+@@@&NASD,AAPL.US}" };
	std::string unsub_msg{ "{(len=29)MARKET01@@S@-@@@&NASD,AAPL.US}" };
};

class event_handler 
{
public:
	event_handler() 
	{ 
		boost::thread th1([this]
		{
			asio::io_context::work worker(context);  
			context.run();
		});
		th1.detach();

		boost::thread th2([this] {
			while (true)
			{
				if (channel_set.size() < max_fds)
				{
					auto ptr = boost::make_shared<tcp_channel>(context);
					channel_set.insert(ptr);
					ptr->start([this, ptr] {
						channel_set.erase(ptr); 
					});
				}
				else
				{
					boost::this_thread::sleep_for(boost::chrono::seconds(1));
				}
			}
		});
		th2.detach();
	}

	~event_handler() 
	{ 
		context.stop();
	}
    
private:

	asio::io_service context;
	std::set<boost::shared_ptr<void>> channel_set;
};

int main(int argc, char *argv[]) {
    signal(SIGINT, handle_sig);
    
    if(argc > 1)
    {
        max_fds = std::stoi(argv[1]);
    }
    std::cout << "max_fds: " << max_fds << std::endl;

    if(argc > 2)
    {
        strncpy(svr_ip, argv[2], sizeof(svr_ip) - 1);
    }
    std::cout << "svr_ip: " << svr_ip << std::endl;

    if(argc > 3)
    {
        port = std::stoi(argv[3]);
    }
    std::cout << "port: " << port << std::endl;

	event_handler eh;

    print_handler();

    return 0;
}

