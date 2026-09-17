module type S = sig type t end
module A = struct module String_id : sig include S end = struct
type t = string end end
