type z = int
module Hash = Hashtbl
module Hash1 : module type of Hash = Hash
