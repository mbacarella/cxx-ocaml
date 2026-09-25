type (+'a, 'b) t and ('a, 'b) u = ('a, 'b) t
type 'a s = 'a list
module K = struct type 'a s  type 'a v = 'a s end
module M = struct type (-'a) m = .. end
type 'a n = 'a M.m
include struct type (-'a) q  type 'a r = 'a q end
