type z = int
module Std = struct module Hash = Hashtbl end
module Hash1 : module type of Std.Hash = Std.Hash
