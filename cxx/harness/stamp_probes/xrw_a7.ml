module rec M : Set.OrderedType with type t = int =
struct type t = int let compare = compare end
and S : sig type u = M.t end = struct type u = M.t end
