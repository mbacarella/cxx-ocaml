let f () =
   let module N = Map.Make(struct type t = int let compare = compare end) in
   ()
