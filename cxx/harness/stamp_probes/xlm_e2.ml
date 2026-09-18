let g () =
   let module N = Map.Make(String) in
   N.empty
type t = int Map.Make(String).t
let f () = 1
