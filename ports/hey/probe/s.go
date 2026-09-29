package main
import ("fmt";"time")
func main(){ for _,ms:=range []int{1,10,50,100}{ t:=time.Now(); for i:=0;i<5;i++{time.Sleep(time.Duration(ms)*time.Millisecond)}; fmt.Println("go",ms,float64(time.Since(t).Microseconds())/5000)} }
