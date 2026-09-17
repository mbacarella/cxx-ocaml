module String_id : sig module type S = sig type t = private string end
include S end = struct module type S = sig type t = private string end
type t = string end
