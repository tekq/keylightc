NAME    := keylightc
VERSION := 0
COMMIT  := $(shell git rev-parse --short HEAD)
RELEASE := $(VERSION).git$(COMMIT)

.PHONY: srpm
srpm:
	git archive --prefix=$(NAME)-$(VERSION)/ --format=tar.gz HEAD \
		-o $(outdir)/$(NAME)-$(VERSION).tar.gz

	cp $(NAME).spec $(outdir)/$(NAME).spec

	rpmbuild -bs \
		--define "_sourcedir $(outdir)" \
		--define "_srcrpmdir $(outdir)" \
		--define "_topdir $(outdir)" \
		$(outdir)/$(NAME).spec
