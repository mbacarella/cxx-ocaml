let g : int Map.Make(String).t -> int = fun _ -> 1
let f () =
   let module N = Map.Make(String) in
   N.add "sum" 41 N.empty
