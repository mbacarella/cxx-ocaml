module A = struct module Str = struct type t = string end
module Make () = struct include Str end end
