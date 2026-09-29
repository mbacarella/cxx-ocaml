module M = Map.Make(Int)
let g () =
   let module N = Map.Make(String) in
   ()
let f () = 1
