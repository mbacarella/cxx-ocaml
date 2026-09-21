module MP = Gc.Memprof
type r = { a : int }
let f () = MP.stop ()
