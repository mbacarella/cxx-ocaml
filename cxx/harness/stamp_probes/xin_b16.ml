module type S = sig type t = private string end
module A = struct module Make (M : sig end) : S = struct type t = string end
end
