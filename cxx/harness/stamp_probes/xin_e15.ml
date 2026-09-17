module type S = sig type t end
module String_id : sig module Make (M : sig end) : S end = struct
module Make (M : sig end) = struct type t = string end end
module Bar = String_id.Make(struct end)
