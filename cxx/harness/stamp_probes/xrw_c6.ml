module rec M : sig type t = int val compare : t -> t -> int end =
struct type t = int let compare = compare end
and S : sig module N : Set.OrderedType end =
struct module N = struct type t = int let compare = compare end end
