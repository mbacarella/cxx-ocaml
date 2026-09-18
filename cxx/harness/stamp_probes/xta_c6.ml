module TT = struct
  module IntSet = Set.Make(struct type t = int let compare = compare end)
end
let f () = let module T = TT in T.IntSet.mem
