type z = int
module Std = struct module Hash = Hashtbl end
open Std
module Hash1 : sig include (module type of Hash) end = Hash
