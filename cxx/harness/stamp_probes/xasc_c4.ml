module M : sig type t = int val compare : t -> t -> int end =
struct type t = int let compare = compare end
let f () =
  let module S : Map.S = Map.Make(M) in
  ignore (S.empty : int S.t)
