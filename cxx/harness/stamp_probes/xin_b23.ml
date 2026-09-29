module A = struct module Make (M : sig val x : int end) : sig
type t = private string type u = t end = struct type t = string type u = t end
end
