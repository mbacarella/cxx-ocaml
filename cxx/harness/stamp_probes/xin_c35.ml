module String_id = struct module Str = struct type t = string end
module Make (M : sig end) = struct include Str end end
