module TT = struct
  module IntSet = Set.Make(struct type t = int let compare = compare end)
end
let () =
  let f flag =
    let module T = TT in
    let _ = T.IntSet.mem in
    () in f `A
