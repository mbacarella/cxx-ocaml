module String_id = struct module type S = sig type t end
module Make (M : sig end) : S = struct type t = string end end
module Bar = String_id.Make(struct end)
