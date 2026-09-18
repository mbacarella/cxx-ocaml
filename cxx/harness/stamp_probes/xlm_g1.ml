module X = struct type t = int let compare = compare end
let f () =
   let module N = Map.Make(X) in
   N.add 1 41 N.empty
