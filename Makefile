.PHONY: all clean db-import db-change db-lexeme source-dump

all: db-import db-change db-lexeme source-dump

db-import:
	$(MAKE) -C src/import

db-change:
	$(MAKE) -C src/change

db-lexeme:
	$(MAKE) -C src/lexeme

source-dump:
	$(MAKE) -C src/dump

clean:
	$(MAKE) -C src/import clean
	$(MAKE) -C src/change clean
	$(MAKE) -C src/lexeme clean
	$(MAKE) -C src/dump clean
