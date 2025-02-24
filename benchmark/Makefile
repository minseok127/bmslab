CXX			:= g++
CXXFLAGS	:= -std=c++20 -O2 -Wall -pthread

TARGET	:= benchmark
SRCS	:= benchmark.cpp

LDFLAGS += -L..
LDLIBS	+= -lbmslab

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SRCS) $(LDFLAGS) -static $(LDLIBS)

clean:
	rm -f $(TARGET)
