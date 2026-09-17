module A = struct module Str = struct type t = string type u = int end
module Make (M : sig end) = struct include Str end end
