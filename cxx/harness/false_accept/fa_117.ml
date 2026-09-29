let rec f = function [] -> "" | h :: t -> h ^ f t let y = f [1]
