package main

import "github.com/valyala/fasthttp"

func main() {
	fasthttp.ListenAndServe(":8202", func(ctx *fasthttp.RequestCtx) {
		ctx.SetContentType("text/plain; charset=utf-8")
		ctx.WriteString("hello")
	})
}
