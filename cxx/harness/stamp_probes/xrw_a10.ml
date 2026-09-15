module rec M : sig type t = int val compare : t -> t -> int end =
struct type t = int let compare = compare end
and S : MoreLabels.Set.OrderedType with type t = M.t =
struct type t = M.t let compare = compare end
