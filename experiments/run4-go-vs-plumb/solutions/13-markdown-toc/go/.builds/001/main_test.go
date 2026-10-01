package main

import "testing"

func TestExample(t *testing.T) {
	src := "# Größe & Maße\n## Setup\n#### Deep\n## Setup ##\n```\n# not a heading\n```\n### Step 1.2: run `make`\n"
	hs := extract(src)
	if len(hs) != 5 {
		t.Fatalf("got %v", hs)
	}
	if s := slug(hs[0].text); s != "größe--maße" {
		t.Fatal(s)
	}
	if hs[3].text != "Setup" || hs[4].text != "Step 1.2: run `make`" {
		t.Fatal(hs)
	}
}

func TestParse(t *testing.T) {
	cases := map[string]string{"# C#": "C#", "### ###": "", "## Title ##": "Title", "# a #\t": "a", "#": ""}
	for in, want := range cases {
		_, got, ok := parseHeading(in)
		if !ok || got != want {
			t.Errorf("%q: %q %v", in, got, ok)
		}
	}
	for _, in := range []string{"#hashtag", "####### x", "    # x"} {
		if _, _, ok := parseHeading(in); ok {
			t.Errorf("%q should not be heading", in)
		}
	}
}
