.PHONY: game run train play clean all

all: game

game:
	$(MAKE) -C game

run:
	$(MAKE) -C game run

train:
	cd ai && python train.py

play:
	cd ai && python play.py

clean:
	$(MAKE) -C game clean
