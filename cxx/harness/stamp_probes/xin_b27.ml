module String_id : sig module type S = sig type t = private string end
module Make (M : sig val module_name : string end) : S end = struct
module type S = sig type t = private string end
module Make (M : sig val module_name : string end) = struct type t = string
end end
