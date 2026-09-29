let f () =
   let module N = Map.Make(String) in
   ignore (N.empty : int N.t)
