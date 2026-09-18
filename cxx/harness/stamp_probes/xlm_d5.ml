let f () =
   let module N = Map.Make(String) in
   N.add "sum" 41 N.empty
type t = int Map.Make(String).t
