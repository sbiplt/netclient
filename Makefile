TARGET = quote_front_client
CXX = g++ 
CXXFLAGS = -std=c++11 -Wall -Os 
LIBS = -lboost_thread -lboost_system

$(TARGET): quote_front_client.o
	$(CXX) $(CXXFLAGS) $(LIBS) $^ -o $@
%.o:%.cpp
	$(CXX) $(CXXFLAGS) $(LIBS) -c $^
clean:
	rm -rf *.o $(TARGET)
