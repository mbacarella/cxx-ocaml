module String_id : sig module type S = sig type t end
module Make (M : sig end) : S end = struct module type S = sig type t end
module Make (M : sig end) = struct type t = string end end
module Bar = String_id.Make(struct end)
