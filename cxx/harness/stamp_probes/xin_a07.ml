module type S = sig type t end
module String_id : sig include S with type t = int end = struct type t = int
end
