module rec M : sig type t = int val compare : t -> t -> int end =
struct type t = int let compare = compare end
and S : Set.S with type elt = M.t = Set.Make(M)
