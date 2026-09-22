module B = struct type t = int end
type u = B.t
module C : sig type t = bool end = struct type t = bool end
type v = C.t
