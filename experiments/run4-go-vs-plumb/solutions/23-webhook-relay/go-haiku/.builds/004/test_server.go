package main

import (
	"flag"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"os/signal"
	"syscall"
)

func init() {
	// Only include this in test builds
	if os.Getenv("BUILD_TEST_SERVER") != "1" {
		return
	}
}

func testServerMain() {
	port := flag.String("port", "9999", "Port to listen on")
	flag.Parse()

	http.HandleFunc("/in", func(w http.ResponseWriter, r *http.Request) {
		body, _ := io.ReadAll(r.Body)
		fmt.Printf("Received: %s\n", string(body))
		fmt.Printf("Headers: %v\n", r.Header)
		w.WriteHeader(http.StatusOK)
		w.Write([]byte("OK"))
	})

	go func() {
		sigChan := make(chan os.Signal, 1)
		signal.Notify(sigChan, syscall.SIGINT, syscall.SIGTERM)
		<-sigChan
		os.Exit(0)
	}()

	addr := "127.0.0.1:" + *port
	log.Printf("Test server listening on %s", addr)
	log.Fatal(http.ListenAndServe(addr, nil))
}
