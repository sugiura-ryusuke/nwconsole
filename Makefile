CC					= gcc
CFLAGS				= -Wall -Wextra
LIBS				= 

TARGET				= $(notdir $(patsubst %/,%,$(dir $(abspath $(firstword $(MAKEFILE_LIST))))))

SRC					= $(TARGET:%=%.c)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) $(LIBS) -o $(TARGET)

all: $(TARGET)

clean:
	@rm -f $(TARGET)

