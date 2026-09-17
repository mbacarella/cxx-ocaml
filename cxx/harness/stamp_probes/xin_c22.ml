module Str = struct type t = string end
module A = struct module B = struct module Make (M : sig end) = struct
include Str end end end
