module Str = struct type t = string end
module A = struct module Make (M : sig end) = struct
module N = struct include Str end end end
