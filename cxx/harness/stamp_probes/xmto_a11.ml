type z = int
module Std = struct module Hash = Hashtbl end
open Std
module Hash1 : module type of Hash = Hash
module Hash2 : sig include (module type of Hash) end = Hash
