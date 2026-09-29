module String_id : sig module type S = sig type t = private string
val of_string : string -> t end module Make (M : sig end) : S end = struct
module type S = sig type t = private string val of_string : string -> t end
module Make (M : sig end) = struct type t = string let of_string s = s end end
