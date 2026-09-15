module rec M : sig type t = int val compare : t -> t -> int end =
struct type t = int let compare = compare end
and S : Hashtbl.HashedType with type t := M.t =
struct let equal (a : M.t) b = a = b let hash (x : M.t) = x end
